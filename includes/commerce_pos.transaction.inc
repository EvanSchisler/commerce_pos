<?php

/**
 * @file
 * Common form-based functions for transactions.
 */

/**
 * Callback for the main POS sale form.
 */
function commerce_pos_sale($form, &$form_state) {
  commerce_pos_transaction_form($form, $form_state, CommercePosService::TRANSACTION_TYPE_SALE);
  return $form;
}

/**
 * Callback for the POS return form.
 */
function commerce_pos_return($form, &$form_state) {
  commerce_pos_transaction_form($form, $form_state, CommercePosService::TRANSACTION_TYPE_RETURN);
  return $form;
}

/**
 * Primary callback for building a POS transaction form.
 */
function commerce_pos_transaction_form(&$form, &$form_state, $transaction_type) {
  global $user;

  $return = $transaction_type == CommercePosService::TRANSACTION_TYPE_RETURN;

  $form['#theme'] = 'commerce_pos_transaction';
  $form['#redirect_key'] = $return ? 'return' : 'sale';
  $form['#transaction_type'] = $transaction_type;

  $form['#attached']['css'][] = drupal_get_path('module', 'commerce_pos') . '/css/commerce_pos_style.css';

  // Attach our CSS and JS.
  $form['#attached']['library'][] = array(
    'system',
    'ui.autocomplete',
  );

  $form['#attached']['library'][] = array(
    'system',
    'drupal.ajax',
  );

  $wrapper_id = 'commerce-pos-sale-wrapper';
  // Lets other modules easily get the wrapper ID.
  $form_state['wrapper_id'] = $wrapper_id;

  $cashier_id = commerce_pos_cashier_get_current_cashier();
  if (!$cashier_id || !commerce_pos_cashier_load($cashier_id)) {
    drupal_set_message(t('You must enter your cashier code to proceed.'), 'warning');
    return;
  }

  commerce_pos_transaction_ajax_check($form, $form_state);

  form_load_include($form_state, 'inc', 'commerce_pos', 'includes/commerce_pos.common');

  if (!empty($form_state['order_updated']) || !isset($form_state['transaction'])) {
    $transaction = CommercePosService::getCurrentTransaction($transaction_type, $cashier_id);

    if (empty($transaction)) {
      $transaction = new CommercePosTransaction(NULL, $transaction_type, $user->uid, $cashier_id);
    }

    $form_state['transaction'] = $transaction;

    if ($order = $form_state['transaction']->getOrder()) {
      if (!empty($form_state['order_updated'])) {
        commerce_order_calculate_total($order);
        commerce_order_save($order);
      }

      $form_state['order_wrapper'] = entity_metadata_wrapper('commerce_order', $order);
    }
    elseif (isset($form_state['order_wrapper'])) {
      unset($form_state['order_wrapper']);
    }
  }

  $order_wrapper = !empty($form_state['order_wrapper']) ? $form_state['order_wrapper'] : FALSE;
  if ($order_wrapper) {
    $line_items = $order_wrapper->commerce_line_items->value();
    $has_active_transaction = !empty($line_items);
  }
  else {
    $has_active_transaction = FALSE;
  }

  $wrapper_ajax = array(
    'callback' => 'commerce_pos_transaction_wrapper_js',
    'wrapper' => $wrapper_id,
  );

  // Determine whether or not the product input should be focused by default.
  if (empty($form_state['values']) || !empty($form_state['focus_product_input'])) {
    $focus_product_input = TRUE;
    unset($form_state['focus_product_input']);
  }
  else {
    $focus_product_input = FALSE;
  }

  $js_settings = array(
    'productAutoCompleteUrl' => url('admin/commerce/pos/product/autocomplete'),
    'focusProductInput' => $focus_product_input,
  );

  $form['#prefix'] = '<div id="' . $wrapper_id . '" class="' . ($return ? 'return' : 'sale') . '">';
  $form['#suffix'] = '</div>';

  if (!empty($form_state['transaction']->transactionId)) {
    $form['transaction_id'] = array(
      '#markup' => '<div class="commerce-pos-transaction-id">' . t('Transaction #@id', array('@id' => $form_state['transaction']->transactionId)) . '</div>',
    );
  }

  // Output a warning if the product types available for POS transactions have
  // not been configured yet.
  $products_not_configured = FALSE;
  $commerce_product_types = commerce_product_types();
  $configured_product_types = variable_get('commerce_pos_available_products', array('product'));
  // If not product types exist.
  if (empty($commerce_product_types)) {
    $products_not_configured = TRUE;
  }
  // If the product types that exist don't match the configured product types.
  else {
    $matching_product = FALSE;
    foreach ($configured_product_types as $key => $product) {
      if (isset($commerce_product_types[$product])) {
        $matching_product = TRUE;
        break;
      }
    }

    if (!$matching_product) {
      $products_not_configured = TRUE;
    }
  }

  if ($products_not_configured) {
    $form['products_not_configured'] = array(
      '#markup' => '<div class="messages warning">'
      . t('Allowed product types have not been configured for POS transactions.
        Click !here to add product types.', array(
          '!here' => l(t('here'), 'admin/commerce/config/pos/settings')
        ))
      . '</div>',
    );
  }

  if (!empty($form_state['lookup_result'])) {
    $form['product_results'] = array(
      '#type' => 'container',
      '#id' => 'commerce-pos-pay-product-results-wrapper',
      '#theme' => 'commerce_pos_product_results',
      '#products' => $form_state['lookup_result'],
    );

    foreach ($form_state['lookup_result'] as $product_id) {
      $form['product_results'][$product_id] = array(
        '#type' => 'button',
        '#value' => t('add'),
        '#name' => 'commerce-pos-pay-product-results-' . $product_id,
        '#product_id' => $product_id,
      );
    }

    unset($form_state['lookup_result']);
  }

  // Let's add a order search textfield if we're in a return transaction so the
  // user could easily pick out which items from the order needs to be returned.
  if ($return) {
    $form['order_search'] = array(
      '#type' => 'container',
      '#prefix' => '<div id="commerce-pos-return-order-search">',
      '#tree' => TRUE,
      '#suffix' => '</div>',
    );
    $form['order_search']['input'] = array(
      '#type' => 'textfield',
      '#title' => '',
      '#size' => 60,
      '#description' => t('Search by order ID to load the items in an order.<br>(Note: This does not support searching for Order Number.)'),
      '#attributes' => array(
        'placeholder' => t('Order Search'),
      ),
      '#default_value' => !empty($form_state['values']['order_search']['input']) ? $form_state['values']['order_search']['input'] : NULL,
    );

    $form['order_search']['search'] = array(
      '#type' => 'button',
      '#value' => t('Go'),
      '#ajax' => $wrapper_ajax,
      '#element_key' => 'do-order-search',
      '#validate' => array('_commerce_pos_order_search_validate'),
      '#name' => 'do-order-search',
    );
  }

  $form['product_search'] = array(
    '#type' => 'container',
  );

  // Add the onboarding text if the order is empty.
  if (empty($order_wrapper)) {
    $form['product_search']['onboarding_text'] = array(
      '#type' => 'markup',
      '#markup' => '<div class="commerce-pos-search-onboarding-message">' . t("<strong>Start your @txn_type:</strong> scan barcode or use product search", array(
        '@txn_type' => $return ? t('return') : t('order'),
      )) . '</div>',
    );
  }

  $form['product_search']['input'] = array(
    '#type' => 'textfield',
    '#title' => t('product search'),
    '#title_display' => 'after',
    '#size' => 60,
    '#description' => t('Search by SKU or product title, start typing to begin your search.'),
    '#attributes' => array(
      'class' => array(
        'commerce-pos-product-autocomplete',
        'commerce-pos-product-search',
      ),
      'placeholder' => t('Product Search'),
    ),
    '#element_validate' => array('_commerce_pos_transaction_validate_product_input'),
  );

  $form['product_search']['product_sku'] = array(
    '#type' => 'textfield',
    '#title' => NULL,
    '#ajax' => $wrapper_ajax,
    '#attributes' => array(
      'class' => array('commerce-pos-product-sku-input'),
    ),
    '#element_key' => 'product-sku',
    '#prefix' => '<div class="element-invisible">',
    '#suffix' => '</div>',
  );

  if (!empty($order_wrapper)) {
    $form['line_items'] = array(
      '#tree' => TRUE,
      '#type' => 'container',
    );

    $total_prefix = $return ? '(' : '';
    $total_suffix = $return ? ')' : '';

    foreach ($order_wrapper->commerce_line_items as $line_item_wrapper) {
      // Only show product line item types.
      if ($line_item_wrapper->type->value() != 'product') {
        continue;
      }

      $line_item_id = $line_item_wrapper->line_item_id->value();

      $form['line_items'][$line_item_id] = array(
        '#type' => 'container',
      );

      $line_item_element = &$form['line_items'][$line_item_id];

      // If commerce_stock is enabled and enabled for this product type as well,
      // make sure we inform the user when a product is out of stock.
      if (!$return && module_exists('commerce_stock') && commerce_ss_product_type_enabled($line_item_wrapper->commerce_product->value()->type)) {
        $out_of_stock = FALSE;
        // Let's check the stock of this product by calling our hook that checks
        // the product's stock.
        // Call all modules that implement the hook, and let them make changes
        // to $stock.
        $stock = module_invoke_all('commerce_stock_check_item_stock_alter', $line_item_wrapper->commerce_product->value());
        // The above function call returns an array.
        if (is_array($stock)) {
          $stock = $stock[0];
        }

        if (is_numeric($stock) && $stock < 1) {
          $out_of_stock = TRUE;
          // If the form has been reloaded, add the existing out of stock items
          // to our array which keeps track of the overridden items so that we
          // don't prompt the user again.
          if (!isset($form_state['values'])) {
            $form_state['out_of_stock_overridden_products'][$line_item_wrapper->commerce_product->product_id->value()] = $line_item_wrapper->commerce_product->product_id->value();
          }
        }

        // If the product has no stock, display a message and buttons to
        // override or remove the product. Do this only once for an item.
        if ($out_of_stock && !isset($form_state['out_of_stock_overridden_products'][$line_item_wrapper->commerce_product->product_id->value()])) {

          // Inform the user about the out of stock issue.
          // Re-use our generic modal form.
          $form['out_of_stock_override_modal'] = commerce_pos_form_modal(array(
            'closeable' => FALSE,
            'throbber' => FALSE
          ));
          $form['out_of_stock_override_modal']['modal']['message'] = array(
            '#markup' => '<div id="out-of-stock-override-confirm-question">'
            . t('This product does not have any stock. Do you still want to add this item to the order?')
            . '</div>',
          );

          // Display a override button to proceed with adding the item to the
          // order.
          $form['out_of_stock_override_modal']['modal']['out_of_stock_override_confirm_yes_button'] = array(
            '#type' => 'button',
            '#value' => t('Override'),
            '#attributes' => array('class' => array('blue')),
            '#ajax' => array(
              'callback' => '_commerce_pos_override_confirm_callback',
              'wrapper' => 'commerce-pos-form-modal-overlay',
              'method' => 'replace'
            ),
            '#product_id' => $line_item_wrapper->commerce_product->product_id->value(),
            '#name' => 'override_button',
            '#validate' => array('_commerce_pos_override_confirm_validate'),
          );

          // Display a remove button to remove the item from the order.
          // We're just copying the same behavior for the generic 'remove' button.
          $form['out_of_stock_override_modal']['modal']['out_of_stock_override_confirm_no_button'] = array(
            '#type' => 'submit',
            '#value' => 'Remove',
            '#attributes' => array('class' => array('link')),
            '#name' => 'line-item-' . $line_item_id . '-remove',
            '#submit' => array('commerce_pos_transaction_line_item_remove_submit'),
            '#line_item_id' => $line_item_id,
            '#ajax' => $wrapper_ajax,
          );
        }
      }

      // Set line item row wrapper class.
      $line_item_row_wrapper_class = "line-item-row-wrapper";

      if (_commerce_pos_product_thumbnail($line_item_wrapper->commerce_product->value())) {
        $line_item_row_wrapper_class .= " line-item-row-wrapper-with-image";
      }

      if (module_exists('commerce_pos_discount')) {
        $line_item_row_wrapper_class .= " line-item-row-with-discount";
      }

      $line_item_element['line_item_row_wrapper_open'] = array(
        '#markup' => '<div class="' . $line_item_row_wrapper_class . '">',
      );

      $line_item_element['image_wrapper_open'] = array(
        '#markup' => '<div class="image-wrapper line-item-col">',
      );

      if ($thumbnail = _commerce_pos_product_thumbnail($line_item_wrapper->commerce_product->value())) {
        $line_item_element['image'] = array(
          '#markup' => $thumbnail,
        );
      }

      $line_item_element['image_wrapper_close'] = array(
        '#markup' => '</div>',
      );

      $line_item_element['title_wrapper_open'] = array(
        '#markup' => '<div class="title-wrapper line-item-col">',
      );

      $line_item_element['title'] = array(
        '#markup' => '<span class="title">' . $line_item_wrapper->commerce_product->title->value() . '</span>',
      );

      if ($display_nid = commerce_pos_get_product_display_nid($line_item_wrapper->commerce_product->product_id->value())) {
        $line_item_element['sku'] = array(
          '#markup' => '<br />' . l($line_item_wrapper->commerce_product->sku->value(), 'node/' . $display_nid, array('attributes' => array('target' => '_blank'))),
        );
      }
      else {
        $line_item_element['sku'] = array(
          '#markup' => '<br />' . $line_item_wrapper->commerce_product->sku->value(),
        );
      }

      // Display out of stock overridden notice.
      if (isset($stock) && $out_of_stock) {
        $line_item_element['out_of_stock_overridden'] = array(
          '#markup' => '<div class="out-of-stock-overridden-notice">' . t('Out of Stock: Overridden') . '</div>',
        );
      }

      $line_item_element['title_wrapper_close'] = array(
        '#markup' => '</div>',
      );

      $base_price = commerce_price_component_load($line_item_wrapper->commerce_unit_price->value(), 'base_price');

      $currency = commerce_currency_load();

      $line_item_element['price_container'] = array(
        '#type' => 'container',
        '#id' => 'commerce-pos-price-container',
        '#attributes' => array(
          'class' => array(
            'commerce-pos-price-container-wrapper price-edit-wrapper line-item-col',
          )
        ),
      );

      $line_item_element['price_container']['price_edit'] = array(
        '#type' => 'textfield',
        '#title' => t('@price', array(
          '@price' => commerce_currency_format($base_price[0]['price']['amount'], $base_price[0]['price']['currency_code']),
        )),
        '#default_value' => number_format($base_price[0]['price']['amount'] / 100, 2, $currency['decimal_separator'], ''),
        '#size' => 5,
        '#maxlength' => 10,
        '#element_key' => 'line-item-change-price',
        '#line_item_id' => $line_item_id,
        '#ajax' => $wrapper_ajax,
        '#attributes' => array(
          'class' => array('commerce-pos-hidden-element'),
          'tabindex' => 0,
        ),
        '#prefix' => '<div class="price-edit-input-wrapper">',
        '#suffix' => '</div>',
      );

      $line_item_element['quantity_wrapper_open'] = array(
        '#markup' => '<div class="quantity-wrapper line-item-col">',
      );

      $line_item_element['textbox_wrapper_open'] = array(
        '#markup' => '<div class="textbox-wrapper">',
      );

      $line_item_element['qty'] = array(
        '#title' => (int) $line_item_wrapper->quantity->value(),
        '#type' => 'textfield',
        '#value' => (int) $line_item_wrapper->quantity->value(),
        '#size' => 4,
        '#maxlength' => 4,
        '#ajax' => $wrapper_ajax,
        '#element_key' => 'line-item-qty',
        '#line_item_id' => $line_item_id,
        '#element_validate' => array('_commerce_pos_transaction_validate_qty'),
        '#attributes' => array(
          'class' => array('commerce-pos-hidden-element'),
        ),
        '#limit_validation_errors' => FALSE,
      );

      $line_item_element['textbox_wrapper_close'] = array(
        '#markup' => '</div>',
      );

      $line_item_element['add_remove_wrapper_open'] = array(
        '#markup' => '<div class="add-remove-wrapper line-item-col">',
      );

      $line_item_element['add_qty'] = array(
        '#type' => 'button',
        '#value' => '+',
        '#name' => 'line-item-' . $line_item_id . '-plus',
        '#element_key' => 'line-item-add-qty',
        '#line_item_id' => $line_item_id,
        '#ajax' => $wrapper_ajax,
        '#attributes' => array(
          'tabindex' => 0,
        ),
      );

      $line_item_element['remove_qty'] = array(
        '#type' => 'button',
        '#value' => '-',
        '#name' => 'line-item-' . $line_item_id . '-minus',
        '#element_key' => 'line-item-remove-qty',
        '#line_item_id' => $line_item_id,
        '#ajax' => $wrapper_ajax,
      );

      $line_item_element['add_remove_wrapper_close'] = array(
        '#markup' => '</div>',
      );

      $line_item_element['quantity_wrapper_close'] = array(
        '#markup' => '</div>',
      );

      $line_item_element['total_wrapper_open'] = array(
        '#markup' => '<div class="total-wrapper line-item-col">',
        '#weight' => 100,
      );

      $line_item_total = $line_item_wrapper->commerce_total->value();

      $line_item_element['total'] = array(
        '#markup' => '<strong>' . $total_prefix . commerce_currency_format($line_item_total['amount'], $line_item_total['currency_code']) . $total_suffix . '</strong>',
        '#weight' => 100,
      );

      $line_item_element['remove'] = array(
        '#type' => 'submit',
        '#value' => 'Remove',
        '#name' => 'line-item-' . $line_item_id . '-remove',
        '#submit' => array('commerce_pos_transaction_line_item_remove_submit'),
        '#line_item_id' => $line_item_id,
        '#ajax' => $wrapper_ajax,
        '#weight' => 100,
      );

      $line_item_element['total_wrapper_close'] = array(
        '#markup' => '</div>',
        '#weight' => 100,
      );

      $line_item_element['line_item_row_wrapper_close'] = array(
        '#markup' => '</div>',
        '#weight' => 100,
      );
    }
  }

  // Transaction options.
  $form['transaction_options'] = array(
    '#type' => 'container',
    '#tree' => TRUE,
    '#id' => 'commerce-pos-transactions-options-wrapper',
  );

  $transaction_options = &$form['transaction_options'];

  if ($has_active_transaction) {
    $order_total = commerce_pos_price_order_format($order_wrapper, 'commerce_order_total', $return);

    $transaction_options['balance'] = array(
      '#markup' => commerce_pos_balance_summary($order_wrapper->value()),
      '#weight' => -9,
    );
  }
  else {
    $order_total = commerce_pos_empty_order_format_total();
  }

  $transaction_options['order_total'] = array(
    '#markup' => render($order_total),
    '#weight' => -10,
  );

  $transaction_options['actions'] = array(
    '#type' => 'action',
  );

  $transaction_options['actions']['void_payment'] = array(
    '#type' => 'submit',
    '#value' => t('Void Payment'),
    '#attributes' => array(
      'class' => array(
        'commerce-pos-transaction-btn',
        'commerce-pos-void-payment',
      ),
      'style' => array('display: none'),
    ),
    '#submit' => array('commerce_pos_transaction_void_payment_submit'),
    '#ajax' => array_merge($wrapper_ajax, array('event' => 'void_payment')),
    '#limit_validation_errors' => array(
      array('transaction_options', 'void_payment_text'),
    ),
    '#element_key' => 'void-payment',
  );

  $transaction_options['void_payment_text'] = array(
    '#type' => 'hidden',
    '#default_value' => NULL,
    '#attributes' => array(
      'class' => array(
        'commerce-pos-void-payment-input',
      ),
      'style' => array('display: none'),
    ),
  );

  if (module_exists('form_keybinds')) {
    $button_keybind_text = ' ' . t('(F4)');
  }
  else {
    $button_keybind_text = '';
  }

  $transaction_options['actions']['pay'] = array(
    '#type' => 'submit',
    '#value' => ($return ? t('Refund') : t('Pay')) . $button_keybind_text,
    '#attributes' => array(
      'class' => array('commerce-pos-btn-pay'),
    ),
    // This won't do anything without form_keybinds installed.
    '#key_binds' => array('f4'),
    '#validate' => array('commerce_pos_transaction_pay_validate'),
    '#submit' => array('commerce_pos_transaction_pay_submit'),
  );

  $transaction_options['actions']['park'] = array(
    '#type' => 'button',
    '#value' => t('Park'),
    '#attributes' => array(
      'class' => array('commerce-pos-transaction-btn'),
    ),
    '#ajax' => $wrapper_ajax,
    '#limit_validation_errors' => array(),
    '#element_key' => 'park-transaction',
  );

  $transaction_options['actions']['void'] = array(
    '#type' => 'button',
    '#value' => t('Void'),
    '#attributes' => array(
      'class' => array('commerce-pos-transaction-btn'),
    ),
    '#ajax' => $wrapper_ajax,
    '#limit_validation_errors' => array(),
    '#element_key' => 'void-transaction',
  );

  $transaction_options['customer'] = array(
    'email' => array(
      '#type' => 'textfield',
      '#title' => t('Customer Info'),
      '#autocomplete_path' => 'admin/commerce/pos/user/autocomplete',
      '#ajax' => array(
        'wrapper' => 'commerce-pos-customer-input-wrapper',
        'callback' => 'commerce_pos_customer_transaction_js',
      ),
      '#prefix' => '<div id="commerce-pos-customer-input-wrapper">',
      '#element_key' => 'customer-update',
      '#description' => t('Email or phone number'),
      '#attributes' => array(
        'placeholder' => t('email@address.com'),
      ),
      '#default_value' => !empty($transaction->data['customer email']) ? $transaction->data['customer email'] : NULL,
    ),
    'phone' => array(
      '#type' => 'textfield',
      '#title' => t('Phone Number'),
      '#suffix' => '</div>',
      '#element_key' => 'customer-update',
      '#description' => t('Record the customers phone number'),
      '#attributes' => array(
        'placeholder' => t('Phone Number'),
      ),
      '#default_value' => !empty($transaction->data['customer phone']) ? $transaction->data['customer phone'] : NULL,
    ),
  );

  if (!$has_active_transaction) {
    $transaction_options['actions']['pay']['#disabled'] = TRUE;
    $transaction_options['actions']['void']['#disabled'] = TRUE;
    $transaction_options['actions']['park']['#disabled'] = TRUE;
  }

  $form['parked_transactions'] = commerce_pos_parked_transactions_section($transaction_type, $wrapper_ajax, $has_active_transaction);

  $form['#attached']['js'][] = drupal_get_path('module', 'commerce_pos') . '/js/commerce_pos.js';
  $form['#attached']['js'][] = drupal_get_path('module', 'commerce_pos') . '/js/keynavigator.js';

  $form['#attached']['js'][] = array(
    'type' => 'setting',
    'data' => array(
      'commercePosSale' => $js_settings,
      '#attributes' => array(
        'class' => array('commerce-pos-transaction-btn'),
      ),
    ),
  );
}

/**
 * Validation handler for the product search input.
 */
function _commerce_pos_transaction_validate_product_input($element, &$form_state, $form) {
  // Always set this input back to empty.
  unset($form_state['input']['input']);
  form_set_value($element, '', $form_state);
}

/**
 * Validation handler for the line item quantity fields.
 */
function _commerce_pos_transaction_validate_qty($element, &$form_state, $form) {
  $requested_qty = drupal_array_get_nested_value($form_state['input'], $element['#parents']);

  // If the increment or decrement buttons were used, we need to add/subtract
  // from the requested_qty.
  if (isset($form_state['triggering_element']['#element_key'])) {
    if ($form_state['triggering_element']['#element_key'] == 'line-item-remove-qty') {
      $requested_qty -= 1;
    }
    if ($form_state['triggering_element']['#element_key'] == 'line-item-add-qty') {
      $requested_qty += 1;
    }
  }

  if (is_numeric($requested_qty) && $requested_qty > 0) {
    form_set_value($element, $requested_qty, $form_state);
  }
  else {
    form_error($element, t('Quantity must be a number and greater than zero.'));
  }
}

/**
 * Submit handler for voiding a payment.
 */
function commerce_pos_transaction_void_payment_submit($form, &$form_state) {
  $transaction_id = $form_state['values']['transaction_options']['void_payment_text'];

  commerce_pos_payment_transaction_void($transaction_id);

  $form_state['rebuild'] = TRUE;
}

/**
 * Helper function to check for AJAX submissions on the POS sale form.
 *
 * This will look at what triggered the AJAX submission and act accordingly.
 */
function commerce_pos_transaction_ajax_check(&$form, &$form_state) {
  if (isset($form_state['triggering_element'])) {
    $triggering_element = $form_state['triggering_element'];

    if (!empty($form_state['triggering_element']['#element_key'])) {
      /* @var CommercePosTransaction $transaction */
      $transaction = $form_state['transaction'];
      $form_state['order_updated'] = TRUE;

      switch ($triggering_element['#element_key']) {
        case 'product-sku':
          if (!empty($form_state['values']['product_sku'])) {
            $product_sku = $form_state['values']['product_sku'];
          }
          elseif (!empty($form_state['input']['product_sku'])) {
            $product_sku = $form_state['input']['product_sku'];
          }
          else {
            $product_sku = FALSE;
          }

          if (!empty($product_sku) && $product = commerce_product_load_by_sku($product_sku)) {
            $transaction->addProduct($product);
          }

          $form_state['focus_product_input'] = TRUE;
          break;

        case 'line-item-qty':
          $line_item_id = $triggering_element['#line_item_id'];
          $transaction->updateLineItemQuantity($line_item_id, $form_state['values']['line_items'][$line_item_id]['qty']);
          break;

        case 'line-item-add-qty':
          $transaction->updateLineItemQuantity($triggering_element['#line_item_id'], 1, 'update');
          break;

        case 'line-item-remove-qty':
          $transaction->updateLineItemQuantity($triggering_element['#line_item_id'], -1, 'update');
          break;

        case 'park-transaction':
          $transaction->doAction('park');
          unset($form_state['transaction']);
          // Need to empty input here as the email and phone numbers are still
          // retaining the old values after rebuilding the form.
          $form_state['input'] = array();
          drupal_set_message(t('Transaction @id parked.', array('@id' => $transaction->transactionId)));
          break;

        case 'void-transaction':
          $transaction->void();
          unset($form_state['transaction']);
          // Need to empty input here as the email and phone numbers are still
          // retaining the old values after rebuilding the form.
          $form_state['input'] = array();
          drupal_set_message(t('Transaction @id voided.', array('@id' => $transaction->transactionId)));
          break;

        case 'retrieve-parked-transaction':
          $parked_transaction = CommercePosService::loadTransaction($triggering_element['#transaction_id']);
          $parked_transaction->unpark();
          $transaction->void();
          unset($form_state['transaction']);

          drupal_set_message(t('Transaction @id retrieved.', array('@id' => $parked_transaction->transactionId)));
          break;

        case 'line-item-change-price':
          // First load the currency array.
          $currency = commerce_currency_load();
          // Strip anything from the input other than numbers or the currency decimal separator.
          $pattern = "/[^0-9" . $currency['decimal_separator'] . "]/";
          $triggering_element['#value'] = preg_replace($pattern, "", $triggering_element['#value']);
          $transaction->setLineItemPrice($triggering_element['#line_item_id'], $triggering_element['#value'] * 100);
          break;

        case 'customer-update':
          $transaction->data['customer email'] = !empty($triggering_element['#value']) ? $triggering_element['#value'] : NULL;
          $transaction->doAction('save');
          break;

        case 'do-order-search':
          $order = commerce_order_load($form_state['input']['order_search']['input']);
          // If we have got a valid order display the item in the order in a
          // modal.
          if ($order) {
            commerce_pos_return_select_item_popup($form, $form_state, $order);
          }
          else {
            form_set_error('', t('The order does not exist.'));
          }

          break;

        case 'return-items-add':
          // Go through the return item checkboxes and get the items from the
          // order that were selected to be returned and add them to the
          // return transaction.
          foreach ($form_state['values']['return_items'] as $key => $value) {
            if (!empty($value)) {
              // We're sending the actual price the customer paid to the
              // addProduct() function, so that price will be used in the
              // transaction instead of the product price.
              $price = $form_state['triggering_element']['#line_items'][$value]->commerce_unit_price->value();
              $transaction->addProduct($form_state['triggering_element']['#line_items'][$value]->commerce_product->value(), 1, TRUE, $price['amount']);
            }
          }

          break;

        default:
          // If we didn't reach any of the above, it means we don't need to reload the order.
          $form_state['order_updated'] = FALSE;
      }

      // See API documentation for an explanation of this hook.
      drupal_alter('commerce_pos_sale_form_ajax', $form_state, $triggering_element);
    }
  }
}

/**
 * Submit handler for removing a line item entirely.
 */
function commerce_pos_transaction_line_item_remove_submit($form, &$form_state) {
  if (!empty($form_state['triggering_element']['#line_item_id'])) {
    $form_state['transaction']->doAction('deleteLineItem', $form_state['triggering_element']['#line_item_id'], TRUE);
    $form_state['transaction']->doAction('saveOrder');
    $form_state['order_updated'] = TRUE;
    $form_state['rebuild'] = TRUE;
  }
}

/**
 * AJAX callback for the customer section.
 */
function commerce_pos_customer_transaction_js($form, &$form_state) {
  return $form['transaction_options']['customer'];
}

/**
 * AJAX callback for returning the sale form's wrapper.
 */
function commerce_pos_transaction_wrapper_js($form, &$form_state) {
  return $form;
}

/**
 * Validation handler for the Sale form's "Pay" button.
 */
function commerce_pos_transaction_pay_validate($form, &$form_state) {
  if (empty($form_state['values']['transaction_options']['customer']['email'])) {
    if (!empty($form_state['values']['transaction_options']['customer']['phone'])) {
      form_error($form['transaction_options']['customer']['email'], t('Please provide an email address with phone number.'));
    }
  }
  else {
    if (!valid_email_address($form_state['values']['transaction_options']['customer']['email'])) {
      form_error($form['transaction_options']['customer']['email'], t('Invalid customer email address.'));
    }
  }
}

/**
 * Submit handler for the Sale form's "Pay" button.
 */
function commerce_pos_transaction_pay_submit($form, &$form_state) {
  if (!empty($form_state['values']['transaction_options']['customer'])) {
    // Attempt to associate the transaction with a customer.
    $email_address = $form_state['values']['transaction_options']['customer']['email'];

    // Pass the phone number along to the payment form.
    if (isset($form_state['values']['transaction_options']['customer']['phone'])) {
      $form_state['transaction']->data['customer phone'] = $form_state['values']['transaction_options']['customer']['phone'];
    }

    if ($user = user_load_by_mail($email_address)) {
      if ($form_state['transaction']->doAction('setOrderCustomer', $user)) {
        // Only save if the order customer was actually updated.
        $form_state['transaction']->doAction('saveOrder');
      }
    }
    if (module_exists('commerce_pos_stock') && $form['#form_id'] === 'commerce_pos_return') {
      $order = commerce_order_load($form_state['transaction']->orderId);
      $order_wrapper = entity_metadata_wrapper('commerce_order', $order);
      foreach ($order_wrapper->commerce_line_items->value() as $line_item) {
        $line_item->data['commerce_pos_stock_add_to_stock'] = $form_state['input']['line_items'][$line_item->line_item_id]['commerce_pos_stock_add_to_stock'];
        commerce_line_item_save($line_item);
      }
    }
  }

  $form_state['transaction']->doAction('save');
  $form_state['redirect'] = 'admin/commerce/pos/' . $form['#redirect_key'] . '/payment';
}

/**
 * Custom ajax callback for the 'Override' button when a product does not have sufficient stock.
 *
 * We just want to remove the entire wrapper div and go on as usual if they
 * click on 'Override'.
 */
function _commerce_pos_override_confirm_callback($form, &$form_state) {
  $commands = array();

  // Remove the entire wrapper and cover.
  $commands[] = ajax_command_replace('#commerce-pos-form-modal-overlay', '');

  return array('#type' => 'ajax', '#commands' => $commands);
}

/**
 * Validate function for the out of stock 'Override' button.
 */
function _commerce_pos_override_confirm_validate($form, &$form_state) {
  // Add the product to the overridden items list.
  $form_state['out_of_stock_overridden_products'][$form_state['triggering_element']['#product_id']] = $form_state['triggering_element']['#product_id'];
}
